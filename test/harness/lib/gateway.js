// What a GraphQL gateway does with an answer, without the gateway: defaultError
// from lib/validations/gql/default.js, ValidationError from verror.js (on
// Error instead of GraphQLError, which only adds locations and path), and
// what graphql-js 16 and the gateway's formatError make of a thrown value.
import { inspect } from 'node:util';

const errorMapping = {
  404: 'NOT_FOUND',
  400: 'BAD_REQUEST',
  409: 'CONFLICT',
  500: 'INTERNAL_SERVER_ERROR'
};

export class ValidationError extends Error {
  constructor (error) {
    super(error.err);
    this.extensions = { code: errorMapping[error.code] || 'INTERNAL_SERVER_ERROR' };
    this.code = error.code;
    this.state = error;
  }
}

export const defaultError = function (data) {
  if (data.code >= 400) {
    throw new ValidationError(data);
  }

  return data;
};

// graphql-js: toError() turns anything that is not an Error into
// "Unexpected error value: <inspect>"; formatError then picks these fields.
export function graphqlView (e) {
  if (!(e instanceof Error)) {
    return { message: 'Unexpected error value: ' + inspect(e).replace(/\s+/g, ' ').slice(0, 60) + '…', notAnError: true };
  }
  return { message: e.message, code: e.code, state: e.state, extensions: e.extensions };
}
